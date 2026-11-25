// Live frames (ipixel-ble encode_live_frame):
//   <len lo> <len hi> 00 00 <option> <total, 4 bytes LE> <JPEG bytes...>
// option 0 = first frame; total is the JPEG length (the app sends a constant
// 12288, which a 48 x 24 panel never treated as complete). Not saved.
#include "live.h"

#include <string.h>
#include "esp_jpeg_common.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "ipixel.h"

static const char *TAG = "live";

#define HEADER 9

#define MAX_JPEG 12000  // one frame; a 192 x 32 picture is a few KB at most
#define QUALITY 80

static jpeg_enc_handle_t s_enc;
static uint8_t *s_rgb;
static uint8_t s_out[HEADER + MAX_JPEG];
static int s_w, s_h, s_last;

uint8_t *live_begin(int width, int height)
{
    if (s_rgb && width == s_w && height == s_h) {
        return s_rgb;
    }
    if (s_enc) jpeg_enc_close(s_enc);
    if (s_rgb) jpeg_free_align(s_rgb);
    s_enc = NULL;
    s_rgb = jpeg_calloc_align(width * height * 3, 16);  // the S3 encoder wants 16-byte alignment
    if (!s_rgb) {
        ESP_LOGE(TAG, "no memory for a %d x %d picture", width, height);
        return NULL;
    }
    // Like Android's Bitmap.compress(JPEG): baseline, 4:2:0, which the panel's
    // decoder is known to take.
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = width;
    cfg.height = height;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB888;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = QUALITY;
    if (jpeg_enc_open(&cfg, &s_enc) != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG encoder would not open for %d x %d", width, height);
        jpeg_free_align(s_rgb);
        s_rgb = NULL;
        return NULL;
    }
    s_w = width;
    s_h = height;
    ESP_LOGI(TAG, "live frames %d x %d", width, height);
    return s_rgb;
}

int live_send(void)
{
    if (!s_enc) return -1;
    int jpeg_len = 0;
    jpeg_error_t e = jpeg_enc_process(s_enc, s_rgb, s_w * s_h * 3, s_out + HEADER, MAX_JPEG, &jpeg_len);
    if (e != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "JPEG encode failed: %d", e);
        return -2;
    }
    int len = HEADER + jpeg_len;
    s_out[0] = len & 0xFF;
    s_out[1] = len >> 8;
    s_out[2] = 0;  // data type 00 00 = live
    s_out[3] = 0;
    s_out[4] = 0;  // first (and only) frame
    // Total length = this JPEG's real length. With the app's constant 12288 the
    // 48 x 24 panel answered "next frame" (1) to every picture, never
    // "complete" (3), and fell back to its saved program between frames.
    s_out[5] = jpeg_len & 0xFF;
    s_out[6] = (jpeg_len >> 8) & 0xFF;
    s_out[7] = (jpeg_len >> 16) & 0xFF;
    s_out[8] = 0;
    s_last = len;
    // Fire-and-forget writes. With confirmed writes the 48 x 24 panel answered
    // every frame but kept showing its saved program.
    return ipixel_write(s_out, len);
}

int live_last_size(void) { return s_last; }
