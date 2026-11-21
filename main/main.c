// iPixel Rhythm: Echo Base mic -> frequency bands -> bars on the iPixel panel over BLE.
// Styles 0..4 use the panel's own rhythm screen (11 levels, panel decides the layout).
// Style 5 draws width/3 bars itself in the panel's live drawing mode.
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "audio.h"
#include "bands.h"
#include "draw.h"
#include "ipixel.h"
#include "ipixel_proto.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "main";

#define FRAME_MS 50         // 20 fps to the panel; ipixel-ble uses 400 ms
#define SILENCE_FRAMES 200  // 10 s of empty bars: stop streaming so the panel shows its own program
#define LOG_EVERY 8         // blocks (~0.25 s) between level prints
#define STYLE_SELF_DRAW IPIXEL_RHYTHM_STYLES  // 5
#define STYLE_COUNT (IPIXEL_RHYTHM_STYLES + 1)

static const uint8_t BRIGHTNESS_STEPS[] = {20, 40, 60, 80, 100};
static volatile uint8_t s_style;
static volatile uint8_t s_brightness;
static volatile bool s_brightness_dirty;

// Latest levels from the audio task: native (11 x 0..15) and self-draw (n x 0..h).
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_native[IPIXEL_RHYTHM_BARS];
static uint8_t s_drawn[BANDS_MAX];
static int s_drawn_count, s_drawn_h;

static void audio_task(void *arg)
{
    static int32_t buf[BANDS_FFT_SIZE];  // 32 ms at 16 kHz
    static bands_t native, drawn;
    uint8_t lv[BANDS_MAX];
    int drawn_w = 0, drawn_h = 0;
    int blocks = 0;

    bands_init(&native, IPIXEL_RHYTHM_BARS, IPIXEL_RHYTHM_LEVEL_MAX);
    for (;;) {
        size_t got = 0;
        while (got < BANDS_FFT_SIZE) {
            got += audio_read(buf + got, BANDS_FFT_SIZE - got);
        }
        bool silent = bands_fft(buf);

        bands_levels(&native, silent, lv);
        taskENTER_CRITICAL(&s_lock);
        memcpy(s_native, lv, IPIXEL_RHYTHM_BARS);
        taskEXIT_CRITICAL(&s_lock);

        // Self-draw bands follow the panel size it reported at connect.
        int w, h;
        if (ipixel_panel_size(&w, &h)) {
            if (w != drawn_w || h != drawn_h) {
                drawn_w = w;
                drawn_h = h;
                bands_init(&drawn, draw_bar_count(w), h);
                ESP_LOGI(TAG, "self-draw: %d bars x %d px", drawn.count, h);
            }
            bands_levels(&drawn, silent, lv);
            taskENTER_CRITICAL(&s_lock);
            memcpy(s_drawn, lv, drawn.count);
            s_drawn_count = drawn.count;
            s_drawn_h = h;
            taskEXIT_CRITICAL(&s_lock);
        }

        if (++blocks % LOG_EVERY == 0) {
            static const char hex[] = "0123456789ABCDEF";
            char line[IPIXEL_RHYTHM_BARS * 2 + 1];
            for (int b = 0; b < IPIXEL_RHYTHM_BARS; b++) {
                line[b * 2] = hex[s_native[b] & 0x0F];
                line[b * 2 + 1] = ' ';
            }
            line[IPIXEL_RHYTHM_BARS * 2 - 1] = 0;
            ESP_LOGD(TAG, "bars [%s]", line);
        }
    }
}

static void on_press(ui_press_t press)
{
    ui_flash();
    if (press == UI_SHORT_PRESS) {
        s_style = (s_style + 1) % STYLE_COUNT;
        ui_set_style(s_style);
        settings_save_style(s_style);
        ESP_LOGI(TAG, "style %u%s", s_style, s_style == STYLE_SELF_DRAW ? " (self-draw)" : "");
    } else {
        int next = 0;
        for (int i = 0; i < (int)sizeof BRIGHTNESS_STEPS; i++) {
            if (BRIGHTNESS_STEPS[i] > s_brightness) { next = i; break; }
        }
        s_brightness = BRIGHTNESS_STEPS[next];
        s_brightness_dirty = true;
        settings_save_brightness(s_brightness);
        ESP_LOGI(TAG, "brightness %u", s_brightness);
    }
}

static bool any_above(const uint8_t *levels, int n, int v)
{
    for (int b = 0; b < n; b++) {
        if (levels[b] > v) return true;
    }
    return false;
}

static void send_task(void *arg)
{
    uint8_t native[IPIXEL_RHYTHM_BARS];
    uint8_t drawn[BANDS_MAX];
    uint8_t cmd[IPIXEL_RHYTHM_LEN];
    bool was_ready = false;
    bool drawing = false;  // panel is in live drawing mode
    bool idle = false;
    int quiet = 0;
    uint32_t sent = 0, failed = 0;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(FRAME_MS));
        if (!ipixel_ready()) {
            was_ready = false;
            drawing = false;
            continue;
        }
        if (!was_ready) {
            uint8_t c[5];
            ipixel_build_power(c, 1);
            ipixel_write(c, sizeof c);
            s_brightness_dirty = true;
            was_ready = true;
            sent = failed = 0;
        }
        if (s_brightness_dirty) {
            uint8_t c[5];
            ipixel_build_brightness(c, s_brightness);
            if (ipixel_write(c, sizeof c) == 0) s_brightness_dirty = false;
        }

        int count, h;
        taskENTER_CRITICAL(&s_lock);
        memcpy(native, s_native, sizeof native);
        count = s_drawn_count;
        h = s_drawn_h;
        memcpy(drawn, s_drawn, count);
        taskEXIT_CRITICAL(&s_lock);

        bool self_draw = s_style == STYLE_SELF_DRAW && count > 0;
        const uint8_t *lv = self_draw ? drawn : native;
        int n = self_draw ? count : IPIXEL_RHYTHM_BARS;

        // Silence: after SILENCE_FRAMES of empty bars stop sending; the panel goes
        // back to its saved program. Wake as soon as a bar shows real sound.
        if (any_above(lv, n, idle ? 1 : 0)) {
            quiet = 0;
            if (idle) {
                idle = false;
                ESP_LOGI(TAG, "sound again, streaming");
            }
        } else if (!idle && ++quiet >= SILENCE_FRAMES) {
            idle = true;
            ESP_LOGI(TAG, "quiet for %d s, pausing", SILENCE_FRAMES * FRAME_MS / 1000);
        }

        // Switch the panel in or out of drawing mode as needed.
        bool want_drawing = self_draw && !idle;
        if (drawing && !want_drawing) {
            draw_leave();
            drawing = false;
        } else if (!drawing && want_drawing) {
            drawing = draw_enter() == 0;
        }
        if (idle) {
            continue;
        }

        int rc;
        if (drawing) {
            int w, ph;
            ipixel_panel_size(&w, &ph);
            rc = draw_frame(drawn, count, w, h);
            if (rc != 0) drawing = false;  // lost pixels: redraw from a clear screen next frame
        } else {
            ipixel_build_rhythm(cmd, s_style, native);
            rc = ipixel_write(cmd, sizeof cmd);
        }
        if (rc == 0) {
            sent++;
        } else {
            failed++;
        }
        if ((sent + failed) % 1000 == 0) {
            ESP_LOGI(TAG, "frames sent %lu, failed %lu", (unsigned long)sent, (unsigned long)failed);
        }
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    if (audio_init() == ESP_OK) {
        xTaskCreatePinnedToCore(audio_task, "audio", 6144, NULL, 6, NULL, 1);
    } else {
        ESP_LOGE(TAG, "mic not started");
    }
    settings_t cfg;
    settings_load(&cfg);
    s_style = cfg.style;
    s_brightness = cfg.brightness;
    ui_set_style(s_style);
    ui_start(on_press);
    ipixel_start(cfg.have_panel ? &cfg.panel : NULL, settings_save_panel);
    xTaskCreatePinnedToCore(send_task, "send", 4096, NULL, 5, NULL, 0);
}
