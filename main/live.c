// Live frames, as the iPixel Color app's DIY image screen sends them on wide
// panels (DiyImageActivity -> SendCore.sendCameraData with raw RGB):
//   <len lo> <len hi> 00 00 <option> <total, 4 bytes LE> <RGB bytes...>
// in 12288-byte blocks, option 0 first then 2; total = width x height x 3
// (AppConfig.ledFrameSize). The panel answers each block: 05 00 00 00 01 (next)
// then 05 00 00 00 02 at the end of the frame. Not saved.
//
// The panel must be in DIY mode first (draw_enter), as the app does: otherwise
// it shows its Bluetooth icon over every frame. A 192 x 32 panel accepted JPEG
// frames too but never showed them.
#include "live.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "ipixel.h"

static const char *TAG = "live";

#define HEADER 9
#define CHUNK 12288

static uint8_t *s_rgb;
static uint8_t s_block[HEADER + CHUNK];
static int s_w, s_h;
static uint32_t s_acks[4], s_timeouts;

uint8_t *live_begin(int width, int height)
{
    if (s_rgb && width == s_w && height == s_h) return s_rgb;
    free(s_rgb);
    s_rgb = calloc(width * height, 3);
    if (!s_rgb) {
        ESP_LOGE(TAG, "no memory for a %d x %d picture", width, height);
        return NULL;
    }
    s_w = width;
    s_h = height;
    ESP_LOGI(TAG, "live frames %d x %d", width, height);
    return s_rgb;
}

int live_send(void)
{
    if (!s_rgb) return -1;
    int len = s_w * s_h * 3;
    for (int off = 0; off < len; off += CHUNK) {
        int n = len - off < CHUNK ? len - off : CHUNK;
        int bl = HEADER + n;
        s_block[0] = bl & 0xFF;
        s_block[1] = bl >> 8;
        s_block[2] = 0;  // data type 00 00 = live
        s_block[3] = 0;
        s_block[4] = off == 0 ? 0 : 2;
        for (int k = 0; k < 4; k++) s_block[5 + k] = (uint8_t)(len >> (8 * k));
        memcpy(s_block + HEADER, s_rgb + off, n);
        ipixel_ack_arm();
        // Paced: unpaced, an 18 KB frame ran NimBLE out of buffers and stalled;
        // confirmed writes worked but managed only 1 frame a second.
        int rc = ipixel_write_paced(s_block, bl);
        if (rc != 0) return rc;
        int st = ipixel_ack_wait(1000);
        if (st < 0) {
            s_timeouts++;
            return -3;
        }
        if (st < 4) s_acks[st]++;
    }
    return 0;
}

void live_acks(uint32_t out[5])
{
    for (int i = 0; i < 4; i++) {
        out[i] = s_acks[i];
        s_acks[i] = 0;
    }
    out[4] = s_timeouts;
    s_timeouts = 0;
}
